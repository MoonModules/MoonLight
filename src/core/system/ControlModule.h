#pragma once

#include "core/util/format.h"   // formatTo: nonblocking formatting into a fixed buffer
#include "core/util/ActiveInstance.h"   // the boot-registry seat, so a surface can find this module
#include "core/util/ControlSurface.h"
#include "core/module/MoonModule.h"
#include "core/util/JsonSink.h"
#include "core/module/Scheduler.h"
#include "core/module/StateDocument.h"   // a preset is a state document
#include "core/util/JsonUtil.h"
#include "core/util/InputMapping.h"   // kTargetTypeMaxNumber: the bank sizes an input row may name
#include "platform/platform.h"

#include <cstdarg>   // setStatusf
#include <cstdio>
#include <algorithm>
#include <cstring>

namespace mm {

/// Puts the device into a named state, and is where anything wanting to do that will live.
///
/// Its first capability is presets: a preset is a file, saving writes one, selecting reads it.
/// Top-level by necessity, since a preset reaches across the containers it captures from.
/// Not to be confused with the fixture profiles module, a library of channel wirings, where this is a device state.
///
/// @moreinfo
///
/// ## The three banks read as one desk
///
/// Switches, encoders and faders are declared in that order, and the declaration order is the render order.
/// The preset grid sits after them rather than between.
/// Eight rows of pads pushed the faders off the bottom of the card, so reaching them meant scrolling past the bank they belong with.
///
/// ## What a preset holds
///
/// A state document, the one `PATCH /api/state` applies, so its top-level keys say which containers it sets.
/// A save writes one container exactly, which decides portability: a look carries nothing about the hardware.
/// So a look applies on any board, where a driver preset carries pins and is specific.
///
/// ## Why files
///
/// One file per preset, with free-form names.
/// Deleting one is deleting a file, and backing them up is copying a folder.
/// Numbered slots would have bought a fixed grid at the cost of both.
/// Applying one is applying a document, the engine every other writer shares.
class ControlModule : public MoonModule, public ListSource {
public:
    /// Where the preset files live, one per preset: @xref{why-files}.
    static constexpr const char* kPresetDir = "/.config/presets";
    /// A preset file's pad, a root `$` key so the engine reads it as the file's own rather than a module.
    static constexpr const char* kSlotKey = "$slot";
    /// The surface is a fixed grid, so a pad has a POSITION rather than a place in a list:
    static constexpr uint8_t kGridCols = 8;
    /// How many rows the pad grid has.
    static constexpr uint8_t kGridRows = 8;
    /// How many presets the grid holds, which is every cell.
    static constexpr uint8_t kMaxPresets = kGridCols * kGridRows;
    /// The longest preset name, which becomes a file name.
    static constexpr uint8_t kMaxNameLen = 32;
    /// The top-level subtrees a preset can carry: @xref{what-a-preset-holds}.
    static constexpr const char* kCapturable[] = {"Layouts", "Effects", "Drivers", "Services"};
    /// What each capturable subtree covers, named after the CONTAINER rather than after a module.
    static constexpr const char* kCaptureRole[] = {"layout", "effects", "driver", "service"};
    static constexpr uint8_t kCaptureCount = sizeof(kCapturable) / sizeof(kCapturable[0]);
    static_assert(sizeof(kCapturable) / sizeof(kCapturable[0]) ==
                  sizeof(kCaptureRole) / sizeof(kCaptureRole[0]),
                  "kCapturable and kCaptureRole are index-aligned");
    /// Index of "Effects" within kCapturable, the role a pure look occupies.
    static constexpr uint8_t kEffectsRole = 1;
    static_assert(kCapturable[kEffectsRole][0] == 'E' && kCapturable[kEffectsRole][1] == 'f' &&
                  kCapturable[kEffectsRole][6] == 's', "kEffectsRole must index Effects");

    /// How many faders the bank shows.
    static constexpr uint8_t kFaderCount = 8;
    /// A row of rotary encoders above the pads, mirroring where both the X-Touch and the QCon put.
    static constexpr uint8_t kEncoderCount = 8;

    /// The switch row.
    static constexpr uint8_t kSwitchCount = 8;
    static_assert(kTargetTypeMaxNumber[1] == kSwitchCount && kTargetTypeMaxNumber[2] == kEncoderCount
                  && kTargetTypeMaxNumber[3] == kFaderCount, "an input row names exactly the surface's banks");
    /// The slots a surface moves, switches first, then faders, then encoders.
    static constexpr uint8_t kSlotCount = kSwitchCount + kFaderCount + kEncoderCount;
    /// Every value one surface can be sent: the slots, then the preset pads.
    static constexpr uint8_t kSurfaceValues = kSlotCount + kMaxPresets;

    /// The boot ControlModule (exactly one exists).
    static ControlModule* active() { return ActiveInstance<ControlModule>::active(); }

    // --- Control surfaces -------------------------------------------------------------------  A.

    /// How a surface attaching learns the values: all at once, one per tick for a network that drops bursts, or not at all for a board following another.
    enum class Seed : uint8_t { Burst, Paced, None };

    /// Attach a surface, seeded as `seed` says.
    void addSurface(ControlSurface* s, Seed seed = Seed::Burst) {
        if (!s) return;
        for (uint8_t i = 0; i < surfaceCount_; i++)
            if (surfaces_[i] == s) return;
        if (surfaceCount_ >= kMaxSurfaces) return;
        touched_[surfaceCount_] = Touch{};
        resendNext_[surfaceCount_] = kSurfaceValues;
        surfaces_[surfaceCount_++] = s;
        // Read the targets BEFORE seeding:
        followTargets();
        if (seed == Seed::Paced) resendPaced(s);
        else if (seed == Seed::Burst) resendTo(s);
    }

    /// Push EVERY value to one surface, whatever the mirror last sent, the preset pads included.
    void resendTo(ControlSurface* s) {
        for (uint8_t n = 0; n < kSurfaceValues; n++) resendOne(s, n);
    }

    /// What a preset pad shows: empty, a stored preset, or the one applied.
    enum PadState : uint8_t { kPadEmpty = 0, kPadStored = 1, kPadActive = 2 };

    /// Every pad's state, one per grid cell, in one pass over the presets.
    void padStates(uint8_t out[kMaxPresets]) const {
        if (padsRemote_) { std::memcpy(out, remotePads_, kMaxPresets); return; }
        std::memset(out, kPadEmpty, kMaxPresets);
        for (uint8_t r = 0; r < presetCount_; r++)
            if (presets_[r].slot < kMaxPresets) out[presets_[r].slot] = padStateOf(presets_[r]);
    }

    /// The state of the pad on grid cell `slot`.
    uint8_t padState(uint8_t slot) const {
        if (padsRemote_) return slot < kMaxPresets ? remotePads_[slot] : static_cast<uint8_t>(kPadEmpty);
        for (uint8_t r = 0; r < presetCount_; r++)
            if (presets_[r].slot == slot) return padStateOf(presets_[r]);
        return kPadEmpty;
    }

    /// Apply the preset on grid cell `slot`, as a click on its pad does; false for an empty cell.
    bool pressPad(uint8_t slot) {
        if (padsRemote_) {
            // The presets are another board's, so the press goes there; true only when a surface sent it.
            if (slot >= kMaxPresets || remotePads_[slot] == kPadEmpty) return false;
            bool sent = false;
            for (uint8_t i = 0; i < surfaceCount_; i++) sent = surfaces_[i]->sendPress(slot) || sent;
            return sent;
        }
        for (uint8_t r = 0; r < presetCount_; r++)
            if (presets_[r].slot == slot) return applyPreset(presets_[r].name);
        return false;
    }

    /// Show another board's pad on the grid, as a board following another does, in place of this board's presets.
    void showRemotePad(uint8_t slot, uint8_t state) {
        if (slot >= kMaxPresets) return;
        if (padsRemote_ && remotePads_[slot] == state) return;
        if (!padsRemote_) std::memset(remotePads_, kPadEmpty, kMaxPresets);
        padsRemote_ = true;
        remotePads_[slot] = state;
        padsRevision_++;
    }

    /// Show this board's own presets again.
    void forgetRemotePads() {
        if (!padsRemote_) return;
        padsRemote_ = false;
        padsRevision_++;
    }

    // A network surface asks for this rather than resendTo, since a burst of datagrams is what WiFi drops.
    /// Push every value to one attached surface, one per tick20ms, starting over if a resend is already running.
    void resendPaced(ControlSurface* s) {
        for (uint8_t i = 0; i < surfaceCount_; i++)
            if (surfaces_[i] == s) resendNext_[i] = 0;
    }

    /// Push value `slot`, counted as kSurfaceValues orders them, to one surface.
    void resendOne(ControlSurface* s, uint8_t slot) {
        if (!s || slot >= kSurfaceValues) return;
        if (slot >= kSlotCount) {
            s->sendValue(SurfaceControl::Pad, slot - kSlotCount, padState(slot - kSlotCount));
            return;
        }
        const SurfaceControl kind = slot < kSwitchCount               ? SurfaceControl::Switch
                                  : slot < kSwitchCount + kFaderCount ? SurfaceControl::Fader
                                                                      : SurfaceControl::Encoder;
        const uint8_t index = slot < kSwitchCount               ? slot
                            : slot < kSwitchCount + kFaderCount ? slot - kSwitchCount
                                                                : slot - kSwitchCount - kFaderCount;
        s->sendValue(kind, index, currentValue(kind, index));
    }

    /// Detach.
    void removeSurface(ControlSurface* s) {
        for (uint8_t i = 0; i < surfaceCount_; i++) {
            if (surfaces_[i] != s) continue;
            --surfaceCount_;
            surfaces_[i] = surfaces_[surfaceCount_];
            touched_[i] = touched_[surfaceCount_];
            resendNext_[i] = resendNext_[surfaceCount_];
            surfaces_[surfaceCount_] = nullptr;
            return;
        }
    }

    /// Set a switch, an encoder or a fader through the control path, so it drives its target exactly as a click does.
    void setValue(SurfaceControl kind, uint8_t index, uint8_t value) {
        const char* control = slotName(kind, index);
        auto* sched = Scheduler::instance();
        if (!control || !sched) return;
        char body[24];
        // A switch takes the literal, since the boolean parser reads true but not 255.
        if (kind == SurfaceControl::Switch) mm::formatTo(body, sizeof(body), "{\"value\":%s}", value ? "true" : "false");
        else mm::formatTo(body, sizeof(body), "{\"value\":%u}", static_cast<unsigned>(value));
        sched->setControl(name(), control, body);
    }

    /// The value a switch, an encoder or a fader holds now.
    uint8_t value(SurfaceControl kind, uint8_t index) const { return currentValue(kind, index); }

    /// A switch's, an encoder's or a fader's control name, null for anything else.
    static const char* slotName(SurfaceControl kind, uint8_t index) {
        switch (kind) {
            case SurfaceControl::Switch:  return index < kSwitchCount ? kSwitchNames[index] : nullptr;
            case SurfaceControl::Encoder: return index < kEncoderCount ? kEncoderNames[index] : nullptr;
            case SurfaceControl::Fader:   return index < kFaderCount ? kFaderNames[index] : nullptr;
            default: return nullptr;
        }
    }

    /// A TURN, not a position.
    void turn(uint8_t index, int8_t delta) {
        if (index >= kEncoderCount) return;
        // THE hardware boundary, and the only place a delta exists.
        const int v = static_cast<int>(encoders_[index]) + delta;
        encoders_[index] = static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
        // driveEncoder clamps to the target's range and stores what it wrote, so a knob turned past.
        driveEncoder(index);
    }

    /// A hand is on this control of surface `by`, which holds back only `by`'s own motor or light; every other surface keeps following.
    void setTouched(ControlSurface* by, SurfaceControl kind, uint8_t index, bool held) {
        for (uint8_t s = 0; s < surfaceCount_; s++) {
            if (surfaces_[s] != by) continue;
            uint32_t* mask = touchMask(s, kind);
            if (!mask || index >= 32) return;
            if (held) { *mask |= (1u << index); return; }
            *mask &= ~(1u << index);
            // Let go: the value moved on while the hand was there, so the surface lands where it ended.
            by->sendValue(kind, index, currentValue(kind, index));
            return;
        }
    }

    /// Push changed values to every attached surface.
    void mirrorToSurfaces() {
        // FOLLOW first, and unconditionally:
        followTargets();
        if (surfaceCount_ == 0) return;
        for (uint8_t i = 0; i < kSwitchCount; i++)
            mirrorOne(SurfaceControl::Switch, i, switches_[i] ? 255 : 0, sentSwitches_[i]);
        for (uint8_t i = 0; i < kEncoderCount; i++)
            mirrorOne(SurfaceControl::Encoder, i, encoders_[i], sentEncoders_[i]);
        for (uint8_t i = 0; i < kFaderCount; i++)
            mirrorOne(SurfaceControl::Fader, i, faders_[i], sentFaders_[i]);
        // The pads change only when a preset is stored, moved, renamed, removed or applied, so they are read only then.
        if (padsMirrored_ == padsRevision_) return;
        padsMirrored_ = padsRevision_;
        uint8_t pads[kMaxPresets];
        padStates(pads);
        for (uint8_t i = 0; i < kMaxPresets; i++) mirrorOne(SurfaceControl::Pad, i, pads[i], sentPads_[i]);
    }

    /// Follow every assignment and push what moved, so a value changing underneath, such as a self-playing game, reaches a desk at once. A pass costs about 1.3 us with all 24 assigned, on a desktop; the preset pads add a pass only when one of them changed.
    void tick20ms() MM_NONBLOCKING override {
        MoonModule::tick20ms();
        mirrorToSurfaces();
        // One value of each paced resend.
        for (uint8_t i = 0; i < surfaceCount_; i++)
            if (resendNext_[i] < kSurfaceValues) resendOne(surfaces_[i], resendNext_[i]++);
    }

    void tick1s() MM_NONBLOCKING override {
        MoonModule::tick1s();
        // The strip falls back to the device's name once what it was showing has gone stale.
        settleStrip();
    }

    /// Declare the strip, the switches, the encoders, the faders, the pads and the save form, in that order: the declaration order is the render order.
    void defineControls() override {
        // The display strip, ABOVE everything:
        controls_.addReadOnly("display", display_, sizeof(display_));
        controls_.setDisplayStrip(controls_.count() - 1);

        // The switch row sits at the TOP, above the encoders, matching the surfaces this mirrors (a.
        for (uint8_t i = 0; i < kSwitchCount; i++) {
            controls_.addControl(kSwitchNames[i], switches_[i]);
            controls_.setSwitchRow(controls_.count() - 1, true, switchTarget(i));
            controls_.setLive(controls_.count() - 1);
        }
        // Encoders next:
        for (uint8_t i = 0; i < kEncoderCount; i++) {
            controls_.addControl(kEncoderNames[i], encoders_[i]);
            controls_.setEncoder(controls_.count() - 1, true, encoderTarget(i));
            controls_.setLive(controls_.count() - 1);
        }
        // The ASSIGNMENTS, one hidden text control per surface control.
        for (uint8_t i = 0; i < kSwitchCount; i++) {
            std::snprintf(targetNames_[i], kTargetNameLen, "%sTarget", kSwitchNames[i]);
            controls_.addText(targetNames_[i], switchTargets_[i], kTargetLen);
            controls_.setHidden(controls_.count() - 1, true);
        }
        for (uint8_t i = 0; i < kEncoderCount; i++) {
            const uint8_t n = kSwitchCount + i;
            std::snprintf(targetNames_[n], kTargetNameLen, "%sTarget", kEncoderNames[i]);
            controls_.addText(targetNames_[n], encoderTargets_[i], kTargetLen);
            controls_.setHidden(controls_.count() - 1, true);
        }
        for (uint8_t i = 0; i < kFaderCount; i++) {
            const uint8_t n = kSwitchCount + kEncoderCount + i;
            std::snprintf(targetNames_[n], kTargetNameLen, "%sTarget", kFaderNames[i]);
            controls_.addText(targetNames_[n], faderTargets_[i], kTargetLen);
            controls_.setHidden(controls_.count() - 1, true);
        }
        // The fader bank, ABOVE the presets list, so the three surface banks read as one desk: @xref{the-three-banks-read-as-one-desk}.
        for (uint8_t i = 0; i < kFaderCount; i++) {
            controls_.addControl(kFaderNames[i], faders_[i]);
            controls_.setFader(controls_.count() - 1, true, surfaceTarget(i));
            controls_.setLive(controls_.count() - 1);
        }
        controls_.addList("presets", *this);
        // The save form: input for the next save rather than configuration, so none of it is written to flash.
        controls_.addText("name", name_, sizeof(name_), validPresetName);
        controls_.setHidden(controls_.count() - 1, true);
        controls_.setLive(controls_.count() - 1);
        // The pad a save is aimed at:
        controls_.addControl("slot", saveSlot_, 0, kMaxPresets - 1);
        controls_.setHidden(controls_.count() - 1, true);
        controls_.setLive(controls_.count() - 1);
        // The module a save writes, by name: a container from the pad editor, any card from its own save button.
        controls_.addText("source", source_, sizeof(source_));
        controls_.setHidden(controls_.count() - 1, true);
        controls_.setLive(controls_.count() - 1);
        controls_.addButton("save");
        controls_.setHidden(controls_.count() - 1, true);
        MoonModule::defineControls();
    }

    /// A preset written or removed from outside (the File Manager, a restore, the gallery) updates its one row.
    void onFileChanged(const char* path) override {
        char name[kMaxNameLen];
        if (holdsPresetFolder(path)) rescan();   // the folder itself, or one holding it, went or came: every row may have changed
        else if (presetNameOf(path, name)) refreshPreset(name);
    }

    /// Whether `path` is the preset folder or a folder above it.
    static bool holdsPresetFolder(const char* path) {
        const size_t n = std::strlen(path);
        return n <= std::strlen(kPresetDir) && std::strncmp(kPresetDir, path, n) == 0 && (kPresetDir[n] == '/' || kPresetDir[n] == 0);
    }

    /// The preset a path names, a `.json` file directly in the folder, into `name`; false for any other path.
    static bool presetNameOf(const char* path, char (&name)[kMaxNameLen]) {
        const size_t dirLen = std::strlen(kPresetDir);
        if (std::strncmp(path, kPresetDir, dirLen) != 0 || path[dirLen] != '/') return false;
        const char* file = path + dirLen + 1;
        const size_t len = std::strlen(file);
        if (len < 6 || len - 5 >= kMaxNameLen || std::strcmp(file + len - 5, ".json") != 0 || std::strchr(file, '/')) return false;
        std::memcpy(name, file, len - 5);
        name[len - 5] = '\0';
        return true;
    }

    /// Take the surface seat, ensure the folder, and scan what is already in it.
    void setup() override {
        // Take the seat before anything looks for us:
        seat_.claim();
        platform::fsMkdir(kPresetDir);
        rescan();
        MoonModule::setup();
    }

    /// `save` writes the current state; a fader drives whatever it targets; the rest is an assignment.
    void onControlChanged(const char* controlName) override {
        if (std::strcmp(controlName, "save") == 0) { savePreset(); return; }
        // An ASSIGNMENT changed:
        if (std::strstr(controlName, "Target") != nullptr) {
            rebuildControls();
            followTargets();
            return;
        }
        for (uint8_t i = 0; i < kFaderCount; i++) {
            if (std::strcmp(controlName, kFaderNames[i]) != 0) continue;
            // Every writer reaches the surfaces, so nothing is marked sent beforehand; a touched control is held back only on the surface touching it.
            driveFader(i);
            // Pushed now rather than at the next 20 ms pass, so a forwarded move arrives at once.
            mirrorOne(SurfaceControl::Fader, i, faders_[i], sentFaders_[i]);
            return;
        }
        for (uint8_t i = 0; i < kEncoderCount; i++) {
            if (std::strcmp(controlName, kEncoderNames[i]) != 0) continue;
            // A transport writes a POSITION, exactly as it does for a fader, and the MOVEMENT is what.
            driveEncoder(i);
            mirrorOne(SurfaceControl::Encoder, i, encoders_[i], sentEncoders_[i]);
            return;
        }
        for (uint8_t i = 0; i < kSwitchCount; i++) {
            if (std::strcmp(controlName, kSwitchNames[i]) != 0) continue;
            // Not marked as already-sent, for the reason the fader branch above gives.
            driveSwitch(i);
            mirrorOne(SurfaceControl::Switch, i, switches_[i] ? 255 : 0, sentSwitches_[i]);
            return;
        }
    }

    // ---- ListSource: one row per preset file ----

    /// How many presets the folder holds.
    uint8_t listRowCount() const override { return presetCount_; }

    /// Append one preset's row: its name, what it carries, and which roles it holds now.
    void writeListRow(JsonSink& sink, uint8_t row) const override {
        if (row >= presetCount_) return;
        const Preset& p = presets_[row];
        // The name is written through writeJsonString, not raw:
        sink.appendf("{\"id\":%lu,\"slot\":%u,\"name\":",
                     static_cast<unsigned long>(p.id), static_cast<unsigned>(p.slot));
        sink.writeJsonString(p.name);
        sink.append(",\"captures\":");
        writeCaptured(sink, p);
        // The roles this preset covers, as role NAMES:
        sink.append(",\"roles\":[");
        bool firstRole = true;
        for (uint8_t i = 0; i < kCaptureCount; i++) {
            if (!(p.roles & (1u << i))) continue;
            sink.appendf("%s\"%s\"", firstRole ? "" : ",", kCaptureRole[i]);
            firstRole = false;
        }
        sink.append("]");
        // Which pad is lit, and for WHICH roles.
        sink.append(",\"activeRoles\":[");
        bool firstActive = true;
        for (uint8_t i = 0; i < kCaptureCount; i++) {
            if (!p.name[0] || std::strcmp(p.name, current_[i]) != 0) continue;
            sink.appendf("%s\"%s\"", firstActive ? "" : ",", kCaptureRole[i]);
            firstActive = false;
        }
        sink.append("]");
        if (!firstActive) sink.append(",\"active\":true");
        sink.append("}");
    }

    /// The expanded row:
    void writeListRowDetail(JsonSink& sink, uint8_t row) const override {
        if (row >= presetCount_) return;
        const Preset& p = presets_[row];
        sink.append("{\"fields\":[{\"name\":\"name\",\"type\":\"text\",\"value\":");
        sink.writeJsonString(p.name);
        sink.append("},{\"name\":\"captures\",\"type\":\"text\",\"readonly\":true,\"value\":");
        writeCaptured(sink, p);
        // refetch: applying a preset rewrites the module tree, so the whole card set is stale.
        sink.append("},{\"name\":\"apply\",\"type\":\"button\",\"label\":\"apply\","
                    "\"refetch\":true}]}");
    }

    // ---- Presets as an external surface (Home Assistant, and any future consumer).

    /// The containers a pad saves whole, named once here for the pad editor.
    void writeListOptionSets(JsonSink& sink) const override {
        sink.append("\"containers\":[");
        for (uint8_t i = 0; i < kCaptureCount; i++) sink.appendf("%s\"%s\"", i ? "," : "", kCapturable[i]);
        sink.append("]");
    }

    /// The one role this preset carries, or kCaptureCount when it sets none or several.
    uint8_t roleOf(uint8_t row) const {
        if (row >= presetCount_) return kCaptureCount;
        uint8_t found = kCaptureCount, n = 0;
        for (uint8_t i = 0; i < kCaptureCount; i++)
            if (presets_[row].roles & (1u << i)) { found = i; n++; }
        return n == 1 ? found : kCaptureCount;
    }

    /// Whether this preset is a pure look, which with one role each means its role is Effects.
    bool isLookOnly(uint8_t row) const { return roleOf(row) == kEffectsRole; }

    /// The preset's name, or null for an out-of-range row.
    const char* presetName(uint8_t row) const {
        return row < presetCount_ ? presets_[row].name : nullptr;
    }

    /// How many presets are on the device.
    uint8_t presetCount() const { return presetCount_; }

    /// Monotonic revision of the preset SET, bumped by every save, delete, rename and rescan.
    uint32_t presetsRevision() const { return presetsRevision_; }

    /// Apply a LOOK by name.
    bool applyLookByName(const char* name) {
        if (!name || !name[0]) return false;
        for (uint8_t i = 0; i < presetCount_; i++) {
            if (std::strcmp(presets_[i].name, name) != 0) continue;
            if (!isLookOnly(i)) return false;
            return applyPreset(presets_[i].name);
        }
        return false;
    }

    /// The look applied most recently, or "" when none is.
    const char* currentLook() const { return current_[kEffectsRole]; }

    /// Editable, since a pad is renamed and deleted from the surface.
    bool isEditableList() const override { return true; }
    /// The folder is the state, read at setup, so the rows are never written to a config file.
    bool persistsList() const override { return false; }

    /// Presets are triggered far more than they are edited, so the rows render as a grid of pads:
    bool listAsPads() const override { return true; }

    /// The surface's shape.
    uint8_t listGridCols() const override { return kGridCols; }
    /// How many rows the surface renders.
    uint8_t listGridRows() const override { return kGridRows; }

    /// Deleting a row deletes the file.
    bool deleteListRow(uint32_t id) override {
        for (uint8_t i = 0; i < presetCount_; i++) {
            if (presets_[i].id != id) continue;
            char path[128];
            pathFor(presets_[i].name, path, sizeof(path));
            // Remember the name before the row goes:
            char goneName[kMaxNameLen];
            std::snprintf(goneName, sizeof(goneName), "%s", presets_[i].name);
            const bool ok = platform::fsRemove(path);
            if (ok) setSurfaceStatusf("deleted %s", goneName);
            else    setStatusf(Severity::Error, "could not delete %s", goneName);
            refreshPreset(goneName);
            return ok;
        }
        return false;
    }

    /// A fader drives its target through Scheduler::setControl, the same domain-neutral primitive.
    const char* surfaceTarget(uint8_t index) const {
        if (index >= kFaderCount || !faderTargets_[index][0]) return nullptr;   // unassigned drives nothing
        return faderTargets_[index];
    }

    /// What a switch drives, as "Module.control", or null when it drives nothing yet.
    const char* switchTarget(uint8_t index) const {
        if (index >= kSwitchCount || !switchTargets_[index][0]) return nullptr;   // unassigned drives nothing
        return switchTargets_[index];
    }

    /// What an encoder drives, as "Module.control", or null when it drives nothing yet.
    const char* encoderTarget(uint8_t index) const {
        if (index >= kEncoderCount || !encoderTargets_[index][0]) return nullptr;   // unassigned drives nothing
        return encoderTargets_[index];
    }

    /// Drives whatever `switchTarget` declares.
    void driveSwitch(uint8_t index) {
        const char* target = switchTarget(index);
        if (!target || index >= kSwitchCount) return;
        const char* dot = std::strchr(target, '.');
        if (!dot) return;
        auto* sched = Scheduler::instance();
        if (!sched) return;
        char module[24];
        const size_t n = std::min(static_cast<size_t>(dot - target), sizeof(module) - 1);
        std::memcpy(module, target, n);
        module[n] = '\0';
        // A button takes every write as a press, so a switch drives one on the way down only.
        const ControlDescriptor* tc = findControl(module, dot + 1);
        if (tc && tc->type == ControlType::Button && !switches_[index]) return;
        // An unchanged target is not rewritten, as in driveSurface.
        int32_t current = 0;
        if (tc && tc->type != ControlType::Button && sched->getControlWide(module, dot + 1, current)
            && (current != 0) == switches_[index]) return;
        char body[32];
        std::snprintf(body, sizeof(body), "{\"value\":%s}", switches_[index] ? "true" : "false");
        sched->setControl(module, dot + 1, body);
        // A bool has no option names, so the strip says on or off rather than 1 or 0:
        writeStrip("%s %s", dot + 1, switches_[index] ? "on" : "off");
    }

    /// Read every bound control back, so a surface FOLLOWS what it drives.
    void followTargets() {
        auto* sched = Scheduler::instance();
        if (!sched) return;
        for (uint8_t i = 0; i < kFaderCount; i++) pullTarget(SurfaceControl::Fader, i, faders_[i]);
        // Encoders follow too.
        for (uint8_t i = 0; i < kEncoderCount; i++) pullTarget(SurfaceControl::Encoder, i, encoders_[i]);
        for (uint8_t i = 0; i < kSwitchCount; i++) {
            uint8_t v = switches_[i] ? 255 : 0;
            if (pullTarget(SurfaceControl::Switch, i, v)) switches_[i] = v != 0;
        }
    }

    /// A target control's descriptor, for its type and bounds.
    static const ControlDescriptor* findControl(const char* moduleName, const char* controlName) {
        auto* sched = Scheduler::instance();
        MoonModule* m = sched ? sched->firstByName(moduleName) : nullptr;
        if (!m) return nullptr;
        const ControlList& cs = m->controls();
        for (uint8_t i = 0; i < cs.count(); i++)
            if (std::strcmp(cs[i].name, controlName) == 0) return &cs[i];
        return nullptr;
    }

    /// One control's read-back.
    bool pullTarget(SurfaceControl kind, uint8_t index, uint8_t& value) {
        const char* target = kind == SurfaceControl::Switch  ? switchTarget(index)
                           : kind == SurfaceControl::Encoder ? encoderTarget(index)
                                                             : surfaceTarget(index);
        if (!target) return false;                    // unassigned: nothing to follow
        const char* dot = std::strchr(target, '.');
        if (!dot) return false;
        auto* sched = Scheduler::instance();
        if (!sched) return false;
        char module[24];
        const size_t n = std::min(static_cast<size_t>(dot - target), sizeof(module) - 1);
        std::memcpy(module, target, n);
        module[n] = '\0';
        uint8_t live = 0;
        if (!sched->getControl(module, dot + 1, live)) return false;
        if (live == value) return false;
        value = live;
        return true;
    }

    /// Write a surface control's value onto whatever it targets.
    void driveSurface(SurfaceControl kind, uint8_t index) {
        const char* target = kind == SurfaceControl::Encoder ? encoderTarget(index)
                                                             : surfaceTarget(index);
        if (!target) return;                          // unassigned
        const char* dot = std::strchr(target, '.');
        if (!dot) return;
        auto* sched = Scheduler::instance();
        if (!sched) return;
        char module[24];
        const size_t n = std::min(static_cast<size_t>(dot - target), sizeof(module) - 1);
        std::memcpy(module, target, n);
        module[n] = '\0';
        uint8_t value = kind == SurfaceControl::Encoder ? encoders_[index] : faders_[index];
        // Clamped to the TARGET's range before writing:
        if (const ControlDescriptor* tc = findControl(module, dot + 1)) {
            const int hi = (tc->type == ControlType::Select || tc->type == ControlType::Palette)
                               ? static_cast<int>(tc->max) - 1 : static_cast<int>(tc->max);
            if (value > hi) value = static_cast<uint8_t>(hi < 0 ? 0 : hi);
            if (value < tc->min) value = tc->min;
        }
        // And the control itself holds what it wrote, so the next read agrees with the target.
        if (kind == SurfaceControl::Encoder) encoders_[index] = value; else faders_[index] = value;
        // An unchanged target is not rewritten: an OSC echo of a sent value would otherwise read as a player taking a self-playing game's control.
        int32_t current = 0;
        if (sched->getControlWide(module, dot + 1, current) && current == value) return;
        char body[32];
        std::snprintf(body, sizeof(body), "{\"value\":%u}", static_cast<unsigned>(value));
        sched->setControl(module, dot + 1, body);
        showOnStrip(module, dot + 1, value);
        followTargets();   // siblings on the same target update now, not at the next pass
    }

    /// Write one fader's value onto whatever it targets.
    void driveFader(uint8_t index)   { driveSurface(SurfaceControl::Fader, index); }
    /// Write one encoder's value onto whatever it targets.
    void driveEncoder(uint8_t index) { driveSurface(SurfaceControl::Encoder, index); }

    /// Put text on the display strip, and start its five-second life.
    void writeStrip(const char* fmt, ...) {
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(display_, sizeof(display_), fmt, ap);
        va_end(ap);
        stripWrittenMs_ = platform::millis();
        stripActive_ = true;
    }

    /// Settle the strip once nothing has happened for a while:
    void settleStrip() {
        if (!stripActive_) return;
        // ELAPSED time, never a comparison of two absolute stamps:
        const uint32_t age = platform::millis() - stripWrittenMs_;
        if (age < kStripHoldMs) return;                        // still showing the change
        const char* name = deviceName();
        // Two holds after the change: the product, then the device. Beyond that, nothing to do.
        if (age < kStripHoldMs * 2)
            mm::formatTo(display_, sizeof(display_), "MoonLight");
        else if (name && name[0])
            mm::formatTo(display_, sizeof(display_), "%s", name);
        else
            mm::formatTo(display_, sizeof(display_), "MoonLight");
    }

    /// The device's name, read through the control system rather than by reaching into SystemModule:
    static const char* deviceName() {
        const ControlDescriptor* c = findControl("System", "deviceName");
        return (c && c->ptr && c->type == ControlType::Text) ? static_cast<const char*>(c->ptr)
                                                             : nullptr;
    }

    /// How long the strip holds what it was told, before falling back to the device's name.
    static constexpr uint32_t kStripHoldMs = 5000;

    /// Write the last thing that happened onto the display strip.
    static bool paletteNameAt(uintptr_t optionsFn, uint8_t index, char* out, size_t outLen) {
        JsonSink sink(out, outLen);
        sink.requestName(index);
        reinterpret_cast<PaletteOptionsFn>(optionsFn)(sink);
        return out[0] != 0 && !sink.overflowed();
    }

    /// Write the last change onto the strip, an option's name rather than its index.
    void showOnStrip(const char* module, const char* control, uint8_t value) {
        auto* sched = Scheduler::instance();
        MoonModule* target = sched ? sched->firstByName(module) : nullptr;
        if (!target) return;
        const ControlList& cs = target->controls();
        for (uint8_t i = 0; i < cs.count(); i++) {
            if (std::strcmp(cs[i].name, control) != 0) continue;
            // A Select carries its options in the descriptor:
            const bool isSelect = cs[i].type == ControlType::Select && cs[i].aux;
            char paletteName[24] = {};
            // "control value", with the name first:
            if (isSelect && value < cs[i].max) {
                const auto* opts = reinterpret_cast<const char* const*>(cs[i].aux);
                writeStrip("%s %s", control, opts[value]);
            } else if (cs[i].type == ControlType::Palette && cs[i].aux && value < cs[i].max
                       && paletteNameAt(cs[i].aux, value, paletteName, sizeof(paletteName))) {
                // The NAME, not "37":
                writeStrip("%s %s", control, paletteName);
            } else {
                writeStrip("%s %u", control, static_cast<unsigned>(value));
            }
            return;
        }
    }

    /// Reorder:
    bool moveListRow(uint32_t id, uint8_t to) override {
        if (to >= kMaxPresets) return false;
        Preset* moving = nullptr;
        for (uint8_t i = 0; i < presetCount_; i++) if (presets_[i].id == id) { moving = &presets_[i]; break; }
        if (!moving) return false;
        if (moving->slot == to) return true;

        Preset* occupant = nullptr;
        for (uint8_t i = 0; i < presetCount_; i++)
            if (presets_[i].slot == to && presets_[i].id != id) { occupant = &presets_[i]; break; }

        const uint8_t from = moving->slot;
        moving->slot = to;
        if (occupant) occupant->slot = from;    // swap rather than overwrite
        // Only the one or two presets whose slot changed get rewritten:
        const bool movedOk = writeSlot(*moving);
        const bool occupantOk = !occupant || writeSlot(*occupant);
        if (!movedOk || !occupantOk) {
            moving->slot = from;
            if (occupant) occupant->slot = to;
            if (movedOk) writeSlot(*moving);            // undo the half that did land
            setStatusf(Severity::Error, "could not save the new pad order");
            sortBySlot();
            return false;
        }
        presetsRevision_++;   // the surface changed: consumers caching the list must re-read
        padsRevision_++;
        sortBySlot();
        return true;
    }

    /// The row's editable fields carry the two actions a preset row needs.
    bool setListRowField(uint32_t id, const char* field, const char* valueJson) override {
        for (uint8_t i = 0; i < presetCount_; i++) {
            if (presets_[i].id != id) continue;
            // `activate` is the pad click and `apply` the row button, one action from two views.
            if (std::strcmp(field, "activate") == 0 || std::strcmp(field, "apply") == 0)
                return applyPreset(presets_[i].name);
            if (std::strcmp(field, "name") == 0) {
                char newName[kMaxNameLen] = {};
                mm::json::parseString(valueJson, "value", newName, sizeof(newName));
                return renamePreset(presets_[i].name, newName);
            }
            return false;
        }
        return false;
    }

private:
    struct Preset {
        uint32_t id = 0;
        char name[kMaxNameLen] = {};
        uint8_t roles = 0;        // bit i: the document sets kCapturable[i]
        bool older = false;       // written in the flat format before presets were documents
        uint8_t slot = 0;         // position on the grid (0..kMaxPresets-1), persisted in the file
        bool hasSlot = false;     // false for a file with no stored slot: assignFreeSlots places it
    };

    /// A stored preset's pad: applied when it is the current preset of any role.
    PadState padStateOf(const Preset& p) const {
        for (uint8_t i = 0; i < kCaptureCount; i++)
            if (p.name[0] && std::strcmp(p.name, current_[i]) == 0) return kPadActive;
        return kPadStored;
    }

    /// Re-read the folder.
    void rescan() {
        presetsRevision_++;   // consumers caching the list re-read it
        padsRevision_++;
        presetCount_ = 0;
        platform::fsList(kPresetDir, &onEntry, this);
        for (uint8_t i = 0; i < presetCount_; i++) readHeader(presets_[i]);
        // A file removed from outside no longer holds its pad.
        for (auto& held : current_)
            if (held[0] && !presetNamed(held)) held[0] = '\0';
        assignFreeSlots();
        sortBySlot();
    }

    /// Re-read one preset after its file changed, which costs one file where a folder walk on an ESP32 costs tens of milliseconds per preset.
    void refreshPreset(const char* name) {
        presetsRevision_++;
        padsRevision_++;
        uint8_t at = 0;
        while (at < presetCount_ && std::strcmp(presets_[at].name, name) != 0) at++;
        char path[128];
        pathFor(name, path, sizeof(path));
        if (platform::fsSize(path) < 0) {   // gone: its row and any pad it held
            if (at < presetCount_) {
                for (uint8_t i = at; i + 1 < presetCount_; i++) presets_[i] = presets_[i + 1];
                presetCount_--;
            }
            clearCurrentIfNamed(name);
            return;
        }
        if (at == presetCount_) {
            if (presetCount_ >= kMaxPresets) return;
            presets_[presetCount_++].id = ++nextId_;
        }
        Preset& p = presets_[at];
        const uint32_t id = p.id;   // the row keeps its id, so the UI keeps it open
        p = Preset{};
        p.id = id;
        std::snprintf(p.name, sizeof(p.name), "%s", name);
        readHeader(p);
        assignFreeSlots();
        sortBySlot();
    }

    /// A preset with no stored slot (saved before slots existed, or copied in by hand) takes the first free pad.
    void assignFreeSlots() {
        bool taken[kMaxPresets] = {};
        for (uint8_t i = 0; i < presetCount_; i++)
            if (presets_[i].hasSlot) taken[presets_[i].slot] = true;
        for (uint8_t i = 0; i < presetCount_; i++) {
            if (presets_[i].hasSlot) continue;
            for (uint8_t s = 0; s < kMaxPresets; s++)
                if (!taken[s]) { presets_[i].slot = s; taken[s] = true; break; }
        }
    }

    void sortBySlot() {
        for (uint8_t i = 1; i < presetCount_; i++) {
            Preset key = presets_[i];
            int j = i - 1;
            while (j >= 0 && presets_[j].slot > key.slot) { presets_[j + 1] = presets_[j]; j--; }
            presets_[j + 1] = key;
        }
    }

    static void onEntry(const char* name, bool isDir, uint32_t /*size*/, void* user) {
        auto* self = static_cast<ControlModule*>(user);
        if (isDir || self->presetCount_ >= kMaxPresets) return;
        const size_t len = std::strlen(name);
        if (len < 6 || std::strcmp(name + len - 5, ".json") != 0) return;   // only our files
        // Skip a name too long to hold, rather than truncating it:
        const size_t stem = len - 5;
        if (stem >= sizeof(Preset::name)) return;
        Preset& p = self->presets_[self->presetCount_];
        p = Preset{};   // the row is reused across scans, and the header fields accumulate
        std::memcpy(p.name, name, stem);
        p.name[stem] = '\0';
        p.id = ++self->nextId_;
        self->presetCount_++;
    }

    /// Drop any active-role claim held by `name`, called when its file goes away.
    void clearCurrentIfNamed(const char* name) {
        for (uint8_t i = 0; i < kCaptureCount; i++)
            if (std::strcmp(current_[i], name) == 0) current_[i][0] = '\0';
        padsRevision_++;
    }

    /// Move an active-role claim to a preset's new name, so a rename does not silently unlight it.
    void renameCurrent(const char* from, const char* to) {
        for (uint8_t i = 0; i < kCaptureCount; i++)
            if (std::strcmp(current_[i], from) == 0)
                std::snprintf(current_[i], sizeof(current_[i]), "%s", to);
        padsRevision_++;
    }

    /// A preset name becomes a FILE name, so it must not be able to steer the path.
    static bool validPresetName(const char* value) {
        if (!value) return false;
        const size_t n = std::strlen(value);
        if (n == 0 || n >= kMaxNameLen) return false;
        for (size_t i = 0; i < n; i++) {
            const unsigned char b = static_cast<unsigned char>(value[i]);
            if (b < 0x20 || b > 0x7E) return false;          // printable ASCII only
            if (b == '/' || b == '\\' || b == '.') return false;   // no separator, no dot-segment
        }
        return true;
    }

    /// Read what a preset sets, from the document's top-level keys, and where its pad is.
    void readHeader(Preset& p) {
        char path[128];
        pathFor(p.name, path, sizeof(path));
        char* body = readFile(path);
        if (!body) return;
        json::JsonDoc doc;
        if (json::parse(body, doc) && doc.rootNode()->type == json::JsonType::Object)
            for (const json::JsonNode* m = doc.node(doc.rootNode()->firstChild); m; m = doc.node(m->next)) readRootKey(p, *m);
        platform::free(body);
    }

    /// What one root key says about a preset: the flat format's header, its pad, or a container it sets.
    static void readRootKey(Preset& p, const json::JsonNode& m) {
        if (std::strcmp(m.key, "captures") == 0) p.older = true;
        if (std::strcmp(m.key, kSlotKey) == 0 && m.type == json::JsonType::Int) {
            p.hasSlot = m.intValue >= 0 && m.intValue < kMaxPresets;
            p.slot = p.hasSlot ? static_cast<uint8_t>(m.intValue) : 0;
        }
        for (uint8_t r = 0; m.type == json::JsonType::Object && r < kCaptureCount; r++)
            if (std::strcmp(m.key, kCapturable[r]) == 0) p.roles |= 1u << r;
    }

    /// A preset file whole, on the heap (caller frees), or null when absent, empty or past the cap.
    static char* readFile(const char* path) {
        const long size = platform::fsSize(path);
        if (size <= 0 || static_cast<size_t>(size) > kStateDocumentMax) return nullptr;
        char* body = static_cast<char*>(platform::alloc(static_cast<size_t>(size) + 1));
        if (!body) return nullptr;
        const int n = platform::fsRead(path, body, static_cast<size_t>(size) + 1);
        if (n <= 0) { platform::free(body); return nullptr; }
        body[n] = '\0';
        return body;
    }

    /// What a row says the preset sets: the containers by name, or that it predates documents.
    static void writeCaptured(JsonSink& sink, const Preset& p) {
        if (p.older) { sink.writeJsonString("an older format"); return; }
        char list[48] = {};
        for (uint8_t i = 0; i < kCaptureCount; i++)
            if (p.roles & (1u << i))
                std::snprintf(list + std::strlen(list), sizeof(list) - std::strlen(list), "%s%s", list[0] ? "," : "", kCapturable[i]);
        sink.writeJsonString(list[0] ? list : "(unknown)");
    }

    /// The preset of this name, or null.
    const Preset* presetNamed(const char* name) const {
        for (uint8_t i = 0; i < presetCount_; i++)
            if (std::strcmp(presets_[i].name, name) == 0) return &presets_[i];
        return nullptr;
    }

    /// Persist the pad order by stamping each file with its position.
    bool writeSlot(const Preset& p) {
        char path[128];
        pathFor(p.name, path, sizeof(path));
        char* body = readFile(path);
        if (!body) return false;
        JsonSink sink;
        restampSlot(sink, body, p.slot);
        platform::free(body);
        return sink.size() > 0 && !sink.overflowed() && platform::fsWriteAtomic(path, sink.data(), sink.size());
    }

    /// A preset file's text with its pad set to `slot`, written ahead of the document in place of any earlier one.
    static void restampSlot(JsonSink& sink, const char* body, uint8_t slot) {
        const char* rest = std::strchr(body, '{');
        if (!rest) return;
        rest = skipSpace(rest + 1);
        // An earlier pad leading the object is dropped, however it is spaced, so repeated reorders never stack pads.
        char prior[16];
        const int priorLen = std::snprintf(prior, sizeof(prior), "\"%s\"", kSlotKey);
        if (std::strncmp(rest, prior, static_cast<size_t>(priorLen)) == 0) {
            const char* p = skipSpace(rest + priorLen);
            if (*p == ':') {
                p = skipSpace(p + 1);
                while (*p == '-' || (*p >= '0' && *p <= '9')) p++;
                p = skipSpace(p);
                rest = *p == ',' ? skipSpace(p + 1) : p;
            }
        }
        sink.appendf("{\"%s\":%u%s", kSlotKey, static_cast<unsigned>(slot), *rest == '}' ? "" : ",");
        sink.append(rest);
    }

    /// `p` past any JSON whitespace.
    static const char* skipSpace(const char* p) {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
        return p;
    }

    static void pathFor(const char* name, char* out, size_t n) {
        std::snprintf(out, n, "%s/%s.json", kPresetDir, name);
    }

    /// Write the source module as a state document.
    void savePreset() {
        // A pad names one save, refused or not, so the next save from a card is never aimed at it.
        struct AimOnce { uint8_t& slot; ~AimOnce() { slot = kNoSlot; } } aimOnce{saveSlot_};
        MoonModule* m = saveSource();
        if (!m) return;
        // A pad can hold one preset:
        if (const Preset* holder = padHolder(saveSlot_); holder && std::strcmp(holder->name, name_) != 0) {
            setStatusf(Severity::Warning, "pad %u is taken by %s", static_cast<unsigned>(saveSlot_ + 1), holder->name);
            return;
        }
        // The file carries the chosen pad; with none chosen, the pad of the preset it overwrites, else the first free one.
        const Preset* same = presetNamed(name_);
        storePreset(*m, saveSlot_ < kMaxPresets ? saveSlot_ : same ? same->slot : kNoSlot);
    }

    /// The module a save writes, or null with the reason shown.
    MoonModule* saveSource() {
        if (name_[0] == 0) { setStatusf(Severity::Warning, "name the preset first"); return nullptr; }
        auto* sched = Scheduler::instance();
        MoonModule* m = sched ? sched->firstByName(source_) : nullptr;
        if (!m) setStatusf(Severity::Error, "%s is not on this device", source_[0] ? source_ : "(no source)");
        return m;
    }

    /// Write `m` as the preset called name_, on pad `slot` when it names one.
    void storePreset(MoonModule& m, uint8_t slot) {
        JsonSink sink;
        if (!writePresetFile(sink, m, slot)) return;
        char path[128];
        pathFor(name_, path, sizeof(path));
        const bool ok = platform::fsWriteAtomic(path, sink.data(), sink.size());
        setStatusf(ok ? Severity::Status : Severity::Error, ok ? "saved %s" : "could not save %s", name_);
        refreshPreset(name_);   // the file carries its slot, so this places it on the clicked pad
    }

    /// The preset on pad `slot`, or null when it is free or no pad is named.
    const Preset* padHolder(uint8_t slot) const {
        for (uint8_t i = 0; slot < kMaxPresets && i < presetCount_; i++)
            if (presets_[i].slot == slot) return &presets_[i];
        return nullptr;
    }

    /// A preset file's text: the pad when one is given, then `m` as a document; false, with the reason shown, when it does not fit.
    bool writePresetFile(JsonSink& sink, MoonModule& m, uint8_t slot) {
        sink.append("{");
        if (slot < kMaxPresets) sink.appendf("\"%s\":%u,", kSlotKey, static_cast<unsigned>(slot));
        writeStateMember(sink, m);
        sink.append("}");
        if (sink.overflowed()) { setStatusf(Severity::Error, "out of memory saving"); return false; }
        if (sink.size() > kStateDocumentMax) { setStatusf(Severity::Error, "%s is larger than a preset holds", m.name()); return false; }
        return true;
    }

    /// Put the device into a preset's state, by applying its document.
    bool applyPreset(const char* presetName) {
        auto* sched = Scheduler::instance();
        if (!sched) return false;
        const Preset* p = presetNamed(presetName);
        if (p && p->older) {
            setStatusf(Severity::Warning, "%s predates presets as documents: Restore a backup to convert it", presetName);
            return false;
        }
        char path[128];
        pathFor(presetName, path, sizeof(path));
        char* body = readFile(path);
        if (!body) { setStatusf(Severity::Error, "could not read %s", presetName); return false; }
        const StateDocumentResult r = applyStateDocument(*sched, body);
        platform::free(body);
        if (!r.ok) {
            setStatusf(Severity::Error, "%s: %s%s%s", presetName, r.error, r.where[0] ? " at " : "", r.where);
            return false;
        }
        sched->prepareTree();
        // The preset now holds each role it sets; the others keep whoever held them, so a look and a layout stay active together.
        for (uint8_t i = 0; p && i < kCaptureCount; i++)
            if (p->roles & (1u << i)) std::snprintf(current_[i], sizeof(current_[i]), "%s", presetName);
        padsRevision_++;
        setSurfaceStatusf("applied %s", presetName);
        return true;
    }

    bool renamePreset(const char* from, const char* to) {
        if (!validPresetName(to)) return false;   // the new name becomes a file name (see validPresetName)
        if (std::strcmp(from, to) == 0) return true;   // renaming to itself is a no-op, not a failure
        char src[128], dst[128];
        pathFor(from, src, sizeof(src));
        pathFor(to, dst, sizeof(dst));
        // Refuse a rename onto an existing preset:
        if (platform::fsSize(dst) >= 0) {
            setStatusf(Severity::Warning, "%s already exists", to);
            return false;
        }
        const long size = platform::fsSize(src);
        if (size <= 0) return false;
        char* buf = static_cast<char*>(platform::alloc(static_cast<size_t>(size) + 1));
        if (!buf) return false;
        const int n = platform::fsRead(src, buf, static_cast<size_t>(size) + 1);
        bool ok = false;
        if (n > 0 && platform::fsWriteAtomic(dst, buf, static_cast<size_t>(n))) {
            // The destination now exists; the rename is only complete once the source is gone.
            if (platform::fsRemove(src)) {
                renameCurrent(from, to);   // the active look follows its new name
                ok = true;
            } else {
                platform::fsRemove(dst);   // roll back, so a half-rename never ships
            }
        }
        platform::free(buf);
        refreshPreset(from);
        refreshPreset(to);
        return ok;
    }

    /// Report through the base's status, the way every module does, so the UI shows it in the card's status chip.
    void setStatusf(Severity sev, const char* fmt, ...) {
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(statusBuf_, sizeof(statusBuf_), fmt, ap);
        va_end(ap);
        setStatus(statusBuf_, sev);
    }

    /// Report a SURFACE action:
    void setSurfaceStatusf(const char* fmt, ...) {
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(statusBuf_, sizeof(statusBuf_), fmt, ap);
        va_end(ap);
        // The STRIP only, not the status row:
        writeStrip("%s", statusBuf_);
    }

    /// Fader names are their position, so a surface binds to "fader1" rather than to a label a user may change.
    static constexpr const char* kFaderNames[kFaderCount] =
        {"fader1", "fader2", "fader3", "fader4", "fader5", "fader6", "fader7", "fader8"};
    static constexpr const char* kEncoderNames[kEncoderCount] =
        {"encoder1", "encoder2", "encoder3", "encoder4", "encoder5", "encoder6", "encoder7", "encoder8"};
    ActiveInstance<ControlModule> seat_{*this};

    /// Attached surfaces. A small fixed array: a rig has a desk and a phone, not thirty.
    static constexpr uint8_t kMaxSurfaces = 4;
    ControlSurface* surfaces_[kMaxSurfaces] = {};
    uint8_t surfaceCount_ = 0;
    /// Per surface, the value a paced resend sends next, kSurfaceValues when none is running.
    uint8_t resendNext_[kMaxSurfaces] = {};
    /// The last value KNOWN to a surface, so only changes go out.
    uint8_t sentSwitches_[kSwitchCount] = {};
    uint8_t sentFaders_[kFaderCount] = {};
    uint8_t sentPads_[kMaxPresets] = {};   ///< per preset pad, the PadState surfaces were last sent
    uint8_t remotePads_[kMaxPresets] = {}; ///< the pads of the board this one follows, while padsRemote_
    bool    padsRemote_ = false;           ///< whether the grid shows another board's presets
    /// One bit per control, per bank, per attached surface: a hand is on it there. See setTouched.
    struct Touch { uint32_t switches = 0, encoders = 0, faders = 0; };
    Touch touched_[kMaxSurfaces] = {};

    uint32_t* touchMask(uint8_t surface, SurfaceControl kind) {
        Touch& t = touched_[surface];
        switch (kind) {
            case SurfaceControl::Switch:  return &t.switches;
            case SurfaceControl::Encoder: return &t.encoders;
            case SurfaceControl::Fader:   return &t.faders;
            default: return nullptr;   // a pad has no travel to fight over
        }
    }

    /// The value a surface control holds now, as surfaces are sent it.
    uint8_t currentValue(SurfaceControl kind, uint8_t index) const {
        switch (kind) {
            case SurfaceControl::Switch:  return index < kSwitchCount && switches_[index] ? 255 : 0;
            case SurfaceControl::Encoder: return index < kEncoderCount ? encoders_[index] : 0;
            case SurfaceControl::Fader:   return index < kFaderCount ? faders_[index] : 0;
            default: return 0;
        }
    }

    /// Push one control if it changed, to every surface without a hand on it.
    void mirrorOne(SurfaceControl kind, uint8_t index, uint8_t value, uint8_t& sent) {
        if (value == sent) return;
        for (uint8_t s = 0; s < surfaceCount_; s++) {
            const uint32_t* mask = touchMask(s, kind);
            if (mask && index < 32 && (*mask & (1u << index))) continue;
            surfaces_[s]->sendValue(kind, index, value);
        }
        sent = value;
    }

    static constexpr const char* kSwitchNames[kSwitchCount] =
        {"switch1", "switch2", "switch3", "switch4", "switch5", "switch6", "switch7", "switch8"};
    char     display_[32] = "MoonLight";   ///< the strip: what the surface last touched, in words
    /// WHEN the strip was last written.
    uint32_t stripWrittenMs_ = 0;
    /// Whether a settle sequence is running.
    bool     stripActive_ = true;
    uint8_t faders_[kFaderCount] = {};
    uint8_t encoders_[kEncoderCount] = {};
    /// What each attached surface was last SENT, so a value it already has is not echoed back.
    uint8_t sentEncoders_[kEncoderCount] = {};

    /// What each surface control drives, as "Module.control", empty when unassigned.
    static constexpr uint8_t kTargetLen = 40;
    /// The control NAMES for those assignments ("fader1Target"), built once and borrowed by the control descriptors.
    static constexpr uint8_t kTargetNameLen = 20;
    char targetNames_[kSwitchCount + kEncoderCount + kFaderCount][kTargetNameLen] = {};
    char faderTargets_[kFaderCount][kTargetLen]     = {"Drivers.brightness"};
    char switchTargets_[kSwitchCount][kTargetLen]   = {"Drivers.on"};
    char encoderTargets_[kEncoderCount][kTargetLen] = {"Drivers.palette"};
    /// bool, not uint8:
    bool switches_[kSwitchCount] = {};
    /// Which pad the next save fills, set by the surface popup.
    static constexpr uint8_t kNoSlot = 0xFF;
    uint8_t saveSlot_ = kNoSlot;

    Preset presets_[kMaxPresets];
    uint8_t presetCount_ = 0;
    uint32_t presetsRevision_ = 0;   ///< see presetsRevision(): drives HA's preset re-fetch
    uint32_t padsRevision_ = 1;      ///< bumped by every change to what a preset pad shows
    uint32_t padsMirrored_ = 0;      ///< the padsRevision_ surfaces were last mirrored at
    uint32_t nextId_ = 0;
    char name_[kMaxNameLen] = {};
    /// The module the next save writes, a look by default.
    char source_[16] = "Effects";
    /// Which preset currently holds each capturable role, index-aligned with kCapturable.
    char current_[kCaptureCount][kMaxNameLen] = {};
    /// Backing store for the status text: setStatus borrows the pointer, so it must outlive the call; sized for a preset name, an error and its full path.
    char statusBuf_[kMaxNameLen + 128] = {};
};

}  // namespace mm
